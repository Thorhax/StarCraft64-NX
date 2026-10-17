#pragma once
// GPU rasterization on one GL thread with a single surfaceless (or pbuffer)
// context. Presentation stays on the libnx framebuffer: with Mesa nouveau on
// Horizon, a window-surface context next to the raster context latched
// "durable GPU submission error -22" in the present path after a few minutes,
// and a single window context made every readback wait for vsync fences.
#include "gfx.hpp"
#include "gl_raster.hpp"
#include "native.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace sc64::nx {
class GlDevice {
public:
    bool create();                   // starts the GL thread; false: software raster
    void destroy();
    // Presentation is not handled here (libnx framebuffer); kept for callers.
    void upload(const sc64::shell::Native::Upload&) {}
    void draw(bool) {}
    // GPU rasterization; services the guest every millisecond while waiting.
    // false: not modelled on the GPU, render in software instead.
    bool render(const gfx::DrawList& list, gfx::SoftwareResult& out, const std::function<void()>& service);
    bool raster_ready() const { return raster_ready_; }
private:
    void loop();
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_, started_;
    std::deque<std::function<void()>> jobs_;
    bool stop_ = false, started_flag_ = false, raster_ready_ = false;
    GlRaster raster_;
};
}
