#pragma once
// EGL + OpenGL presentation on the default NWindow (Mesa: Zink on NVK).
// Step 2 of the GPU renderer: same image and Fit geometry as the libnx
// framebuffer path, drawn as a textured quad. Rasterization is unchanged.
#include "native.hpp"
#include <optional>

namespace sc64::nx {
class GlVideo {
public:
    bool create();                  // false: caller falls back to the framebuffer
    void destroy();
    void upload(const sc64::shell::Native::Upload& image);
    void draw(bool visible);        // render and eglSwapBuffers (vsync paced)
    void* display() const { return display_; }
    void make_current();            // bind this context and the window surface on the calling thread
    void* config() const { return config_; }
private:
    void* display_ = nullptr;
    void* config_ = nullptr;
    void* surface_ = nullptr;
    void* context_ = nullptr;
    unsigned program_ = 0, vao_ = 0, texture_ = 0;
    int rect_location_ = -1;
    unsigned texture_w_ = 0, texture_h_ = 0;
    bool dirty_ = false;
    std::optional<sc64::shell::Native::Upload> image_;
};
}
