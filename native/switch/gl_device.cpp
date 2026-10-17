#include "gl_device.hpp"
#include "log.hpp"
#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <chrono>
#include <future>

namespace sc64::nx {
bool GlDevice::create() {
    thread_ = std::thread([this] { loop(); });
    std::unique_lock lock(mutex_);
    started_.wait(lock, [this] { return started_flag_; });
    if (!raster_ready_) { lock.unlock(); thread_.join(); }
    return raster_ready_;
}

void GlDevice::loop() {
    // Core 2 (the guest Gfx thread there is mostly idle), above the raster helpers.
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1u << 2);
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
    using sc64::shell::log::info;
    using sc64::shell::log::warning;
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface pbuffer = EGL_NO_SURFACE;
    bool current = false;
    if (display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API)) {
        const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        const EGLint context_attributes[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                                             EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (eglChooseConfig(display, config_attributes, &config, 1, &count) && count > 0)
            context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        if (context != EGL_NO_CONTEXT) {
            // Surfaceless first (EGL_KHR_surfaceless_context), then a 16x16 pbuffer.
            if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) && eglGetCurrentContext() == context) {
                current = true;
                info("GL raster: surfaceless OpenGL 4.3 context (presentation on the libnx framebuffer)");
            } else {
                const EGLint surface_attributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
                pbuffer = eglCreatePbufferSurface(display, config, surface_attributes);
                if (pbuffer != EGL_NO_SURFACE && eglMakeCurrent(display, pbuffer, pbuffer, context)
                    && eglGetCurrentContext() == context) {
                    current = true;
                    info("GL raster: pbuffer OpenGL 4.3 context (presentation on the libnx framebuffer)");
                }
            }
        }
    }
    if (!current) warning("GL raster: no OpenGL context (0x%x), software raster", unsigned(eglGetError()));
    const bool raster = current && raster_.init();
    {
        std::lock_guard lock(mutex_);
        raster_ready_ = raster;
        started_flag_ = true;
    }
    started_.notify_all();
    if (raster) {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (jobs_.empty()) break;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
        raster_.shutdown();
    }
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (pbuffer != EGL_NO_SURFACE) eglDestroySurface(display, pbuffer);
        eglTerminate(display);
    }
}

void GlDevice::destroy() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_one();
    thread_.join();
}

bool GlDevice::render(const gfx::DrawList& list, gfx::SoftwareResult& out, const std::function<void()>& service) {
    if (!raster_ready_) return false;
    std::promise<bool> done;
    auto future = done.get_future();
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back([this, &list, &out, &done] {
            bool ok = false;
            try { ok = raster_.render(list, out); }
            catch (const std::exception& error) {
                sc64::shell::log::warning("GL raster: %s", error.what());
            }
            done.set_value(ok);
        });
    }
    wake_.notify_one();
    while (future.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
        if (service) service();
    return future.get();
}
}
