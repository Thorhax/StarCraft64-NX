#include <thread>
#include "flash_device.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using sc64::runtime::FlashDevice;
namespace {
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
template<typename Callable> void rejects(Callable call, const char* message) {
    bool rejected = false;
    try { call(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("test requires an output directory");
        const std::filesystem::path storage(argv[1]);
        std::filesystem::create_directories(storage);
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto root = storage / ("case-" + std::to_string(unique));
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("test case directory already exists");
        const auto path = root / "flash.bin";
        FlashDevice device;
        device.initialize(path);
        const auto initial = device.snapshot();
        require(std::all_of(initial.begin(), initial.end(), [](auto value) { return value == 0xff; }), "erased cold FlashRAM");
        require(!std::filesystem::exists(path), "cold read must not create save");
        device.command(0xe1000000);
        require(device.read_pio(0) == 0x11118001, "silicon ID type");
        std::array<std::uint8_t, 8> id{};
        device.read_dma(0, id);
        require(id[4] == 0 && id[5] == 0xc2 && id[6] == 0 && id[7] == 0x1d, "reference Macronix maker/model");
        device.command(0xd2000000);
        require(device.read_pio(0) == 0x11118001, "first D2 must retain previous mode");
        device.command(0xd2000000);
        require(device.read_pio(0) == 0x008c008c, "second D2 completed ready status");
        require(device.read_pio(0x20000) == 0xffffffff, "status high address open bus");
        std::array<std::uint8_t, FlashDevice::page_size> page{};
        for (std::size_t index = 0; index < page.size(); ++index) page[index] = static_cast<std::uint8_t>(index);
        device.command(0xb4000000);
        device.write_dma(0, page);
        device.command(0xa50003ff);
        require(std::filesystem::file_size(path) == FlashDevice::capacity, "program persists complete image");
        device.command(0xf0000000);
        std::array<std::uint8_t, FlashDevice::page_size> actual{};
        device.read_dma(0x1ff80, actual);
        require(actual == page, "last page array read");
        require(device.read_pio(0x1ff80) == 0x00010203, "PIO guest word order");
        page.fill(0xf0);
        device.command(0xb4000000);
        device.write_dma(0, page);
        device.command(0xa50003ff);
        auto programmed = device.snapshot();
        require(programmed[0x1ff8f] == 0 && programmed[0x1ffff] == 0x70, "Flash programming cannot restore cleared bits");
        const auto previous_file = read_file(path);
        std::filesystem::create_directory(root / "flash.bin.pending");
        device.command(0x4b0003ff);
        rejects([&] { device.command(0x78000000); }, "failed save write must throw");
        require(read_file(path) == previous_file, "failed publication preserved valid prior save");
        require(device.snapshot() == programmed, "failed publication preserved current memory");
        std::filesystem::remove(root / "flash.bin.pending");
        const std::vector<std::uint8_t> candidate(FlashDevice::capacity, 0x5a);
        rejects([&] { sc64::runtime::write_verified_file(path, candidate, [](const std::filesystem::path& pending) {
            std::fstream corrupt(pending, std::ios::binary | std::ios::in | std::ios::out);
            corrupt.seekp(77); corrupt.put('\0'); }); }, "corrupted pending image must not publish");
        require(read_file(path) == previous_file, "verification failure preserved valid prior save");
        require(!std::filesystem::exists(root / "flash.bin.pending"), "verification failure removes pending image");
        rejects([&] { sc64::runtime::write_verified_file(path, candidate, [](const std::filesystem::path& pending) {
            std::filesystem::resize_file(pending, FlashDevice::capacity - 1); }); }, "short pending image must not publish");
        require(read_file(path) == previous_file, "short verification failure preserved valid prior save");
        device.command(0x78000000);
        const auto erased = device.snapshot();
        require(std::all_of(erased.begin() + 0x1c000, erased.end(), [](auto value) { return value == 0xff; }), "last sector erase includes final byte");
        FlashDevice reopened;
        reopened.initialize(path);
        require(reopened.snapshot() == erased, "cold persistence round trip");
        { std::ofstream broken(root / "short.bin", std::ios::binary); broken.put('x'); }
        rejects([&] { reopened.initialize(root / "short.bin"); }, "reject incomplete save rather than silently fill");
        require(reopened.snapshot() == erased, "failed initialization must not mutate prior valid model");
        rejects([&] { reopened.command(0x99000000); }, "unsupported command must fail explicitly");
        rejects([&] { reopened.read_pio(1); }, "PIO alignment checked");
        std::vector<std::uint8_t> rdram(0x800000);
        auto& shared = sc64::runtime::flash_device();
        shared.initialize(root / "shared.bin");
        sc64_flash_pio_write(0x08010000, 0xb4000000);
        for (std::size_t index = 0; index < 128; ++index) rdram[(0x400 + index) ^ 3] = static_cast<std::uint8_t>(index ^ 0xa5);
        sc64_flash_dma(rdram.data(), 0x80000400, 0x08000000, 128, 1);
        sc64_flash_pio_write(0x08010000, 0xa5000000);
        sc64_flash_pio_write(0x08010000, 0xf0000000);
        sc64_flash_dma(rdram.data(), 0xa0000800, 0x08000000, 128, 0);
        for (std::size_t index = 0; index < 128; ++index) require(rdram[(0x800 + index) ^ 3] == (index ^ 0xa5), "DMA byte-swapped RDRAM read/write");
        rejects([&] { sc64_flash_dma(rdram.data(), 0x807ffff0, 0x08000000, 32, 0); }, "guest boundary rejected");
        rejects([&] { sc64_flash_dma(rdram.data(), 0x80000000, 0x10000000, 4, 0); }, "physical device boundary rejected");
        {
            // Deferred persistence: a burst of page programs is one file write after it goes quiet.
            const auto deferred_path = root / "deferred.bin";
            FlashDevice deferred;
            deferred.initialize(deferred_path);
            deferred.defer_writes(std::chrono::milliseconds(100));
            for (std::uint32_t page = 0; page < 400; ++page) {
                deferred.command(0xb4000000);
                std::array<std::uint8_t, 128> bytes;
                bytes.fill(std::uint8_t(page));
                deferred.write_dma(0, bytes);
                deferred.command(0xa5000000 | (page & 0x3ff));
            }
            require(!std::filesystem::exists(deferred_path), "deferred image is not written during the burst");
            const auto programmed = deferred.snapshot();
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            FlashDevice quiet;
            quiet.initialize(deferred_path);
            require(quiet.snapshot() == programmed, "deferred image written once quiet");
            deferred.command(0xb4000000);
            std::array<std::uint8_t, 128> last;
            last.fill(0x5a);
            deferred.write_dma(0, last);
            deferred.command(0xa5000000);
            deferred.finish(); // shutdown before the quiet period ends
            FlashDevice finished;
            finished.initialize(deferred_path);
            require(finished.snapshot() == deferred.snapshot(), "finish publishes the pending image");
        }
        std::cout << "PASS: Flash protocol, end boundaries, atomic failure, read-back verification, cold reload, guest DMA byte order, deferred writes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
