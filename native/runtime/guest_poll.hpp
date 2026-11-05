#pragma once
#include <ultramodern/ultramodern.hpp>
#include <atomic>
#include <cstdint>
#include <thread>

extern std::atomic_bool exited;

namespace sc64::runtime {
// Cooperative scheduling boundary for statically recompiled guest loops.
// Pause points are profile-specific: the shell session calls its own pause
// point first; the capture host deliberately has none.
inline void guest_boundary(std::uint8_t* rdram, std::uint32_t millis) {
    if (exited.load()) throw ultramodern::thread_terminated{};
    ultramodern::wait_for_external_message_timed(rdram, millis);
    ultramodern::check_running_queue(rdram);
}
// D-023: the original priority-zero idle loop is preempted by interrupts.
// Static C needs an explicit cooperative scheduling boundary at that loop.
inline void idle_boundary(std::uint8_t* rdram) { guest_boundary(rdram, 1); }
// D-025: let the original VI worker advance its own software counter.
// The waiting CPU loop must yield to process queued host events.
inline void busy_wait_boundary(std::uint8_t* rdram) {
    guest_boundary(rdram, 0);
    std::this_thread::yield();
}
// The same loops in the shell, without spinning (ADR-093): the polled state changes
// only after an external message (the VI interrupt) wakes the guest thread that
// writes it, so wait up to 1 ms for one instead of returning at once. Runnable
// guest threads are scheduled before the wait as after it.
inline void blocking_wait_boundary(std::uint8_t* rdram) {
    if (exited.load()) throw ultramodern::thread_terminated{};
    ultramodern::check_running_queue(rdram);
    guest_boundary(rdram, 1);
}
// D-024: busy polling must observe the real two-entry guest AI FIFO.
// It also provides an interrupt/scheduling boundary while the CPU waits.
template<class Status>
std::uint32_t ai_status_boundary(std::uint8_t* rdram, Status&& status_source) {
    if (exited.load()) throw ultramodern::thread_terminated{};
    const std::uint32_t status = status_source();
    if (status & 0x80000000u) guest_boundary(rdram, 1);
    return status;
}
// D-016: nominal 60 Hz HLE field phase, not physical-console certification.
// VI_CURRENT has a parity bit; the game masks it in scanline wait loops.
inline std::uint64_t vi_current_line(std::int64_t elapsed_ns, std::uint64_t field) {
    const std::uint64_t phase = std::uint64_t(elapsed_ns) % 16'666'667;
    return ((phase * 525 / 16'666'667) & ~1ull) | (field & 1);
}
}
