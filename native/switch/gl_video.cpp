#include "gl_video.hpp"
#include "log.hpp"
#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <algorithm>
#include <cstdint>

namespace sc64::nx {
namespace {
constexpr unsigned screen_w = 1280, screen_h = 720;

const char* vertex_source = R"(#version 330 core
uniform vec4 rect; // x0, y0, x1, y1 in NDC (y up)
out vec2 uv;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    uv = vec2(corner.x, 1.0 - corner.y); // image row 0 is the top
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);
}
)";
const char* fragment_source = R"(#version 330 core
uniform sampler2D image;
in vec2 uv;
out vec4 color;
void main() { color = vec4(texture(image, uv).rgb, 1.0); }
)";

GLuint compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char text[1024] = {};
        glGetShaderInfoLog(shader, sizeof(text), nullptr, text);
        sc64::shell::log::error("GL shader compile failed: %s", text);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
}

bool GlVideo::create() {
    using sc64::shell::log::info;
    using sc64::shell::log::warning;
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
        warning("GL: EGL display unavailable (0x%x)", unsigned(eglGetError()));
        return false;
    }
    display_ = display;
    if (!eglBindAPI(EGL_OPENGL_API)) { warning("GL: eglBindAPI failed"); destroy(); return false; }
    const EGLint config_attributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint configs = 0;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &configs) || configs < 1) {
        warning("GL: no RGBA8 window config (0x%x)", unsigned(eglGetError()));
        destroy(); return false;
    }
    EGLSurface surface = eglCreateWindowSurface(display, config,
                                                reinterpret_cast<EGLNativeWindowType>(nwindowGetDefault()), nullptr);
    if (surface == EGL_NO_SURFACE) {
        warning("GL: eglCreateWindowSurface failed (0x%x)", unsigned(eglGetError()));
        destroy(); return false;
    }
    surface_ = surface;
    config_ = config;
    EGLContext context = EGL_NO_CONTEXT;
    const EGLint versions[][2] = {{4, 3}, {3, 3}}; // 4.3 core as in CBO1-NX, then 3.3 core
    for (const auto& version : versions) {
        const EGLint context_attributes[] = {
            EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
            EGL_CONTEXT_MAJOR_VERSION_KHR, version[0], EGL_CONTEXT_MINOR_VERSION_KHR, version[1], EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        if (context != EGL_NO_CONTEXT) {
            info("GL: OpenGL %d.%d core context", int(version[0]), int(version[1]));
            break;
        }
    }
    if (context == EGL_NO_CONTEXT) { warning("GL: eglCreateContext failed (0x%x)", unsigned(eglGetError())); destroy(); return false; }
    context_ = context;
    // eglMakeCurrent's result is checked explicitly (lesson from CBO1-NX).
    if (!eglMakeCurrent(display, surface, surface, context) || eglGetCurrentContext() != context) {
        warning("GL: eglMakeCurrent failed (0x%x)", unsigned(eglGetError()));
        destroy(); return false;
    }
    // Interval 0: eglSwapBuffers must not block the GL thread on vsync (the
    // guest waits for raster jobs on that thread). GlDevice paces the caller
    // to 60 Hz and the system compositor still flips on vsync.
    eglSwapInterval(display, 0);

    const GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment) { destroy(); return false; }
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) { sc64::shell::log::error("GL: program link failed"); destroy(); return false; }
    rect_location_ = glGetUniformLocation(program_, "rect");
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "image"), 0);
    glGenVertexArrays(1, &vao_); // core profile draws need a bound VAO
    glBindVertexArray(vao_);
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    info("GL: presenting through %s, %s", renderer ? renderer : "?", version ? version : "?");
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
        warning("GL: setup error 0x%x", unsigned(error));
        destroy(); return false;
    }
    return true;
}

void GlVideo::make_current() {
    eglMakeCurrent(static_cast<EGLDisplay>(display_), static_cast<EGLSurface>(surface_),
                   static_cast<EGLSurface>(surface_), static_cast<EGLContext>(context_));
}

void GlVideo::destroy() {
    auto* display = static_cast<EGLDisplay>(display_);
    if (!display) return;
    if (context_ && eglGetCurrentContext() == static_cast<EGLContext>(context_)) {
        if (texture_) glDeleteTextures(1, &texture_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (program_) glDeleteProgram(program_);
    }
    texture_ = vao_ = program_ = 0;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context_) eglDestroyContext(display, static_cast<EGLContext>(context_));
    if (surface_) eglDestroySurface(display, static_cast<EGLSurface>(surface_));
    eglTerminate(display);
    display_ = surface_ = context_ = nullptr;
}

void GlVideo::upload(const sc64::shell::Native::Upload& image) {
    image_ = image;
    dirty_ = true;
}

void GlVideo::draw(bool visible) {
    // The GPU rasterizer shares this context: restore presentation state.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(program_);
    glBindVertexArray(vao_);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glViewport(0, 0, screen_w, screen_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (visible && image_ && image_->rgba && image_->width && image_->height
        && image_->rgba->size() >= std::size_t(image_->width) * image_->height * 4) {
        const unsigned w = image_->width, h = image_->height;
        glBindTexture(GL_TEXTURE_2D, texture_);
        if (dirty_) {
            if (w != texture_w_ || h != texture_h_) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(w), GLsizei(h), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                             image_->rgba->data());
                texture_w_ = w; texture_h_ = h;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE,
                                image_->rgba->data());
            }
            dirty_ = false;
        }
        // Fit: the same geometry as the framebuffer path and the touch mapping.
        unsigned dw = screen_w, dh = unsigned(std::uint64_t(screen_w) * h / w);
        if (dh > screen_h) { dh = screen_h; dw = unsigned(std::uint64_t(screen_h) * w / h); }
        const float ox = float(screen_w - dw) / 2, oy = float(screen_h - dh) / 2;
        const float x0 = ox / screen_w * 2 - 1, x1 = (ox + dw) / screen_w * 2 - 1;
        const float y_top = 1 - oy / screen_h * 2, y_bottom = 1 - (oy + dh) / screen_h * 2;
        glUniform4f(rect_location_, x0, y_bottom, x1, y_top);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    eglSwapBuffers(static_cast<EGLDisplay>(display_), static_cast<EGLSurface>(surface_));
}
}
