#!/usr/bin/env python3
"""Derive the Switch runtime copy from the guarded shell runtime.

local/deps/N64ModernRuntime-shell stays untouched (Android/desktop builds keep
their hash guards); this writes a fresh local/deps/N64ModernRuntime-switch with
small, asserted __SWITCH__ branches.
"""
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'local/deps/N64ModernRuntime-shell'
TARGET = ROOT / 'local/deps/N64ModernRuntime-switch'

PATCHES = [
    ('ultramodern/include/ultramodern/renderer_context.hpp',
     '#elif defined(__linux__) || defined(__ANDROID__)\n        using WindowHandle = SDL_Window*;',
     '#elif defined(__SWITCH__)\n        using WindowHandle = void*;\n'
     '#elif defined(__linux__) || defined(__ANDROID__)\n        using WindowHandle = SDL_Window*;'),
    ('ultramodern/src/error_handling.cpp',
     '#ifdef __APPLE__\n    std::_Exit(exit_status);',
     '#if defined(__APPLE__) || defined(__SWITCH__)\n    std::_Exit(exit_status);'),
    ('ultramodern/src/threads.cpp',
     '#elif defined(__APPLE__)\nvoid ultramodern::set_native_thread_name',
     '#elif defined(__SWITCH__)\n'
     '// Horizon keeps a thread on its creating core (libnx default: core 0) and does not\n'
     '// time-slice equal priorities, so spread threads by role over the 3 application cores.\n'
     'extern "C" uint32_t svcSetThreadCoreMask(uint32_t handle, int32_t core, uint32_t mask);\n'
     'extern "C" uint32_t svcSetThreadPriority(uint32_t handle, uint32_t priority);\n'
     'void ultramodern::set_native_thread_name(const std::string& name) {\n'
     '    // Role table (lower priority value = more urgent; shell main thread: core 0, 0x2C).\n'
     '    // Equal priorities on one core never time-slice on Horizon, so every thread\n'
     '    // that shares a core with busy work must sit above it.\n'
     '    int core = 1; uint32_t priority = 0x2C;                        // guest threads\n'
     '    if (name == "VI Thread") { core = 0; priority = 0x28; }\n'
     '    else if (name == "Timer Thread") { core = 0; priority = 0x29; }\n'
     '    else if (name == "SP Task Thread") { core = 1; priority = 0x2A; } // short audio tasks ahead of guest code\n'
     '    else if (name == "Gfx Thread") { core = 2; priority = 0x2C; }     // renderer owns core 2\n'
     '    svcSetThreadCoreMask(0xFFFF8000u, core, 1u << core);\n'
     '    svcSetThreadPriority(0xFFFF8000u, priority);\n'
     '}\n'
     'void ultramodern::set_native_thread_priority(ThreadPriority) {} // set by role above\n'
     '#elif defined(__APPLE__)\nvoid ultramodern::set_native_thread_name'),
    ('thirdparty/concurrentqueue/lightweightsemaphore.h',
     '#elif defined(__unix__)\n#include <semaphore.h>',
     '#elif defined(__SWITCH__)\n#include <chrono>\n#include <condition_variable>\n#include <mutex>\n'
     '#elif defined(__unix__)\n#include <semaphore.h>'),
    ('thirdparty/concurrentqueue/lightweightsemaphore.h',
     '#elif defined(__unix__)\n//---------------------------------------------------------\n// Semaphore (POSIX, Linux)',
     '#elif defined(__SWITCH__)\n'
     '// Semaphore (Switch: portable mutex/condition variable)\n'
     'class Semaphore\n{\nprivate:\n'
     '\tstd::mutex m_mutex;\n\tstd::condition_variable m_cv;\n\tstd::uint64_t m_count;\n'
     '\tSemaphore(const Semaphore& other) = delete;\n'
     '\tSemaphore& operator=(const Semaphore& other) = delete;\n'
     'public:\n'
     '\tSemaphore(int initialCount = 0) : m_count(static_cast<std::uint64_t>(initialCount)) {}\n'
     '\tbool wait() { std::unique_lock<std::mutex> lock(m_mutex); m_cv.wait(lock, [&] { return m_count > 0; }); --m_count; return true; }\n'
     '\tbool try_wait() { std::lock_guard<std::mutex> lock(m_mutex); if (!m_count) return false; --m_count; return true; }\n'
     '\tbool timed_wait(std::uint64_t usecs) { std::unique_lock<std::mutex> lock(m_mutex);\n'
     '\t\tif (!m_cv.wait_for(lock, std::chrono::microseconds(usecs), [&] { return m_count > 0; })) return false;\n'
     '\t\t--m_count; return true; }\n'
     '\tvoid signal() { { std::lock_guard<std::mutex> lock(m_mutex); ++m_count; } m_cv.notify_one(); }\n'
     '\tvoid signal(int count) { if (count <= 0) return; { std::lock_guard<std::mutex> lock(m_mutex); m_count += static_cast<std::uint64_t>(count); } m_cv.notify_all(); }\n'
     '};\n'
     '#elif defined(__unix__)\n//---------------------------------------------------------\n// Semaphore (POSIX, Linux)'),
    ('librecomp/include/librecomp/addresses.hpp',
     '    constexpr size_t mem_size = 512ULL * 1024ULL * 1024ULL;',
     '#ifdef __SWITCH__\n'
     '    // Switch: committed for real (no lazy mmap). 8 MiB RDRAM + PI handles,\n'
     '    // patch data and a 16 MiB recomp heap above mod_rdram_start.\n'
     '    constexpr size_t mem_size = 32ULL * 1024ULL * 1024ULL;\n'
     '#else\n'
     '    constexpr size_t mem_size = 512ULL * 1024ULL * 1024ULL;\n'
     '#endif'),
    ('librecomp/src/recomp.cpp',
     '#else\n#    include <sys/mman.h>\n#endif',
     '#elif !defined(__SWITCH__)\n#    include <sys/mman.h>\n#endif'),
    ('librecomp/src/recomp.cpp',
     '#else\n    rdram = (uint8_t*)mmap(NULL, allocation_size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);',
     '#elif defined(__SWITCH__)\n'
     '    rdram = static_cast<uint8_t*>(std::aligned_alloc(0x1000, mem_size));\n'
     '    alloc_failed = rdram == nullptr;\n'
     '    if (!alloc_failed) std::memset(rdram, 0, mem_size);\n'
     '#else\n    rdram = (uint8_t*)mmap(NULL, allocation_size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);'),
    ('librecomp/src/recomp.cpp',
     '#else\n    // munmap returns -1 on failure.',
     '#elif defined(__SWITCH__)\n    std::free(rdram);\n    free_failed = false;\n'
     '#else\n    // munmap returns -1 on failure.'),
    ('librecomp/src/mods.cpp',
     '#else\n#  include <unistd.h>\n#  include <dlfcn.h>',
     '#elif defined(__SWITCH__)\n'
     '// Switch: no dynamic code mods. Every library reports "not loaded".\n'
     'class recomp::mods::DynamicLibrary {\npublic:\n'
     '    static constexpr std::string_view PlatformExtension = ".so";\n'
     '    DynamicLibrary() = default;\n'
     '    DynamicLibrary(const std::filesystem::path&) {}\n'
     '    DynamicLibrary(const DynamicLibrary&) = delete;\n'
     '    DynamicLibrary& operator=(const DynamicLibrary&) = delete;\n'
     '    void unload() {}\n'
     '    bool good() const { return false; }\n'
     '    template <typename T> bool get_dll_symbol(T& out, const char*) const { out = nullptr; return false; }\n'
     '    uint32_t get_api_version() { return (uint32_t)-1; }\n'
     '};\n'
     'void unprotect(void*, uint64_t* old_flags) { *old_flags = 0; }\n'
     'void protect(void*, uint64_t) {}\n'
     '#else\n#  include <unistd.h>\n#  include <dlfcn.h>'),
    ('thirdparty/sse2neon/sse2neon.h',
     '    if (!posix_memalign(&ptr, align, size))\n        return ptr;\n    return NULL;',
     '#if defined(__SWITCH__)\n'
     '    // newlib on Switch has no posix_memalign; C11 aligned_alloc wants a size multiple of align.\n'
     '    ptr = aligned_alloc(align, (size + align - 1) / align * align);\n'
     '    return ptr;\n'
     '#else\n'
     '    if (!posix_memalign(&ptr, align, size))\n        return ptr;\n    return NULL;\n'
     '#endif'),
]


def main() -> None:
    if not (SOURCE / 'sc64-shell-patch.json').is_file():
        sys.exit(f'Missing {SOURCE}; prepare the shell runtime first')
    if TARGET.exists():
        shutil.rmtree(TARGET)
    shutil.copytree(SOURCE, TARGET, symlinks=True)
    for name, old, new in PATCHES:
        path = TARGET / name
        text = path.read_text()
        if text.count(old) != 1:
            sys.exit(f'Patch anchor not unique/found in {name}: {old.splitlines()[0]!r}')
        path.write_text(text.replace(old, new))
    print(f'Prepared Switch runtime: {TARGET.relative_to(ROOT)} ({len(PATCHES)} patches)')


if __name__ == '__main__':
    main()
