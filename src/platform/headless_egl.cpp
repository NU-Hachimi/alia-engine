#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdio>

#include "platform/surface.hpp"

namespace alia::platform {
namespace {

class HeadlessEglSurface final : public Surface {
public:
    ~HeadlessEglSurface() override {
        if (dpy_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (ctx_ != EGL_NO_CONTEXT) eglDestroyContext(dpy_, ctx_);
            eglTerminate(dpy_);
        }
    }

    bool init(const SurfaceDesc& d, std::string* error) {
        w_ = d.width;
        h_ = d.height;
        // Surfaceless platform: no window system needed (works over ssh/adb, in CI, on the
        // board without a display). Fall back to the default display if it's unavailable.
        auto get_platform_display =
            reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (get_platform_display) {
            dpy_ = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
            platform_ = "surfaceless";
        }
        if (dpy_ == EGL_NO_DISPLAY) {
            dpy_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            platform_ = "default";
        }
        if (dpy_ == EGL_NO_DISPLAY || !eglInitialize(dpy_, nullptr, nullptr)) return fail(error, "eglInitialize");
        if (!eglBindAPI(EGL_OPENGL_ES_API)) return fail(error, "eglBindAPI");

        const EGLint cfg_attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
        EGLConfig cfg;
        EGLint n = 0;
        // No config is fine with EGL_KHR_no_config_context; try a config first for portability.
        EGLConfig chosen = (eglChooseConfig(dpy_, cfg_attrs, &cfg, 1, &n) && n > 0) ? cfg : EGL_NO_CONFIG_KHR;
        const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
        ctx_ = eglCreateContext(dpy_, chosen, EGL_NO_CONTEXT, ctx_attrs);
        if (ctx_ == EGL_NO_CONTEXT) return fail(error, "eglCreateContext");
        // EGL_KHR_surfaceless_context: current with no draw surface; we render to FBOs only.
        if (!eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx_)) return fail(error, "eglMakeCurrent");
        return true;
    }

    int width() const override { return w_; }
    int height() const override { return h_; }
    bool has_default_framebuffer() const override { return false; }
    bool poll_events() override { return true; }
    void present() override {}

    std::string describe() const override {
        const char* vendor = eglQueryString(dpy_, EGL_VENDOR);
        return std::string("headless (EGL ") + platform_ + ", " + (vendor ? vendor : "?") + ")";
    }

private:
    bool fail(std::string* error, const char* what) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s failed (EGL error 0x%x)", what, eglGetError());
        *error = buf;
        return false;
    }

    EGLDisplay dpy_ = EGL_NO_DISPLAY;
    EGLContext ctx_ = EGL_NO_CONTEXT;
    int w_ = 0, h_ = 0;
    const char* platform_ = "";
};

}  // namespace

std::unique_ptr<Surface> create_headless_surface(const SurfaceDesc& desc, std::string* error) {
    auto s = std::make_unique<HeadlessEglSurface>();
    if (!s->init(desc, error)) return nullptr;
    return s;
}

}  // namespace alia::platform
